// Built water: the wells and water barrels players build (Prop_NPC_Water_Well_a/b_PLAYERCRAFTABLE, the craftable water
// barrel, ...). They are not in the world's scene data, so the mod finds them at run time among the client's ECS
// entities: every entity offering the game's "Drink from" interaction (ClientInteractionOffer.verbId = loca id
// 0xc2f79d24) that is not one of the world's wells or barrels (ore_deposits.h). The client only holds the entities near
// the player, so they are remembered per world in looted_chests.ini (BuiltWater = x,y,z;...) and forgotten once the
// player is close and they are gone. The Map Key's Water Source row shows and counts them with the world's water.
//
// The registry: ECS systems call ViewInit with an iterator whose *iter + 0x30 is their entity registry; the calls of a
// few client systems that run every frame (and two interaction ones) are redirected to learn it, keeping up to four
// registries and using the one that holds the drinkable entities (ViewInit itself stays unhooked: XHL's Area Harvest
// reads its code, and Quick Deposit redirects other systems' calls the same way).
// Registry layout (FUN_1409a72a0, GetEntityComponent): entity map at +0x148 (occupancy bits *(map + 0x08), capacity
// map + 0x10, entity ids *(map + 0x28)), entity records *(registry + 0x188). A record: +0x18 archetype (640 type bits,
// then a u16 offset per type at +0x84 and a u16 size per type at +0xa84), +0x20 chunk data, +0x30 row. A component is
// chunk data + offset + row * size.
// Component type numbers change between game sessions, so CurrentTransform (int64 x, y, z with 32 fraction bits, 0x38
// bytes) and ClientInteractionOffer (0xc bytes, verbId first) are recognised by their data.

bool GetHudPlayerWorldPos(uint8_t* client, float& outX, float& outZ);

struct BuiltWaterSpot {
    float x, y, z;
};
std::vector<BuiltWaterSpot> g_builtWater; // this world's
bool g_builtWaterDirty = false;

using EcsViewInitFn = void (*)(void* iter, void* view, uint32_t size);
EcsViewInitFn g_origEcsViewInit = nullptr;
constexpr int kMaxWaterRegistries = 4;
std::atomic<void*> g_waterRegistries[kMaxWaterRegistries];
std::atomic<void*> g_waterRegistry{nullptr}; // the one with the drinkable entities
constexpr uint32_t kDrinkFromVerb = 0xc2f79d24u;
constexpr int kEcsTypeBits = 640;

void LearnWaterRegistry(void* iter) {
    void* reg = nullptr;
    __try {
        reg = *reinterpret_cast<void**>(*static_cast<uint8_t**>(iter) + 0x30);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    for (auto& slot : g_waterRegistries) {
        if (!reg) break;
        void* cur = slot.load(std::memory_order_relaxed);
        if (cur == reg) break;
        if (!cur) {
            void* expected = nullptr;
            if (slot.compare_exchange_strong(expected, reg) || expected == reg) break;
        }
    }
}

void WaterViewInitHook(void* iter, void* view, uint32_t size) {
    {
        greeneye::Lease lease; // an older generation after a hot reload only calls on
        if (lease && !g_modOff && !g_waterRegistries[kMaxWaterRegistries - 1].load(std::memory_order_relaxed)) {
            LearnWaterRegistry(iter);
        }
    }
    g_origEcsViewInit(iter, view, size);
}

void InstallBuiltWaterHooks() {
    uint8_t* viewInit = FindUnique("48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60");
    if (!viewInit) {
        if (g_log) g_log("Built water: ViewInit not found; wells and barrels players build are not shown");
        return;
    }
    static const char* const kSystems[] = {"render_transform", "lerp_render_transform", "update_ui_data",
                                           "client_cursor_entity", "client_interaction_toggle_new",
                                           "ui_possible_interactions"};
    constexpr int kSystemCount = sizeof(kSystems) / sizeof(kSystems[0]);
    int calls = 0, systems = 0;
    g_origEcsViewInit = reinterpret_cast<EcsViewInitFn>(
        RedirectSystemCalls(viewInit, kSystems, kSystemCount, reinterpret_cast<void*>(&WaterViewInitHook), calls, systems));
    if (g_log) {
        g_log("Built water: %d ViewInit calls redirected in %d of 6 systems%s", g_origEcsViewInit ? calls : 0, systems,
              g_origEcsViewInit ? "" : " (failed: built wells and barrels are not shown)");
    }
}

// A component of an entity record, or nullptr; size gets the component's size.
const uint8_t* EcsComponent(uint8_t* rec, uint16_t type, uint16_t* size = nullptr) {
    uint8_t* arch = *reinterpret_cast<uint8_t**>(rec + 0x18);
    if (!arch || !(reinterpret_cast<const uint64_t*>(arch)[type >> 6] >> (type & 63) & 1)) return nullptr;
    const uint16_t off = *reinterpret_cast<const uint16_t*>(arch + 0x84 + type * 2);
    const uint16_t sz = *reinterpret_cast<const uint16_t*>(arch + 0xa84 + type * 2);
    if (size) *size = sz;
    uint8_t* data = *reinterpret_cast<uint8_t**>(rec + 0x20);
    return data ? data + off + static_cast<size_t>(*reinterpret_cast<const uint32_t*>(rec + 0x30)) * sz : nullptr;
}

// Calls f(record) for every non-player entity of the registry.
template <class F>
void ForEachEcsEntity(uint8_t* reg, F&& f) {
    uint8_t* map = reg + 0x148;
    const uint64_t capacity = *reinterpret_cast<uint64_t*>(map + 0x10);
    auto* used = *reinterpret_cast<uint64_t**>(map + 0x08);
    auto* ids = *reinterpret_cast<uint32_t**>(map + 0x28);
    auto* records = *reinterpret_cast<uint8_t***>(reg + 0x188);
    if (!used || !ids || !records || capacity > (1u << 22)) return;
    for (uint64_t i = 0; i < capacity; ++i) {
        if (!(used[i >> 6] >> (i & 63) & 1)) continue;
        if (ids[i] < 0x400 || !records[i]) continue; // player entities use ids 1..0x3ff
        f(records[i]);
    }
}

bool EcsPosition(uint8_t* rec, uint16_t type, float out[3]) {
    uint16_t size = 0;
    const uint8_t* p = EcsComponent(rec, type, &size);
    if (!p || size != 0x38) return false;
    for (int i = 0; i < 3; ++i) out[i] = static_cast<float>(reinterpret_cast<const int64_t*>(p)[i] / 4294967296.0);
    return true;
}

bool PlausibleWorldPos(const float p[3]) {
    return p[0] > 50.0f && p[0] < 10300.0f && p[1] > -300.0f && p[1] < 3000.0f && p[2] > 50.0f && p[2] < 10300.0f;
}

// The two component types, recognised by their data (once per registry).
struct WaterEcsTypes {
    void* registry = nullptr;
    uint16_t position = 0, offer = 0;
};
WaterEcsTypes g_waterTypes;

bool DetectWaterTypesInner(uint8_t* reg) {
    if (g_waterTypes.registry != reg) g_waterTypes = WaterEcsTypes{reg, 0, 0};
    if (!g_waterTypes.position) {
        int counts[kEcsTypeBits]{};
        int sampled = 0;
        ForEachEcsEntity(reg, [&](uint8_t* rec) {
            if (sampled >= 3000) return;
            ++sampled;
            const auto* bits = *reinterpret_cast<const uint64_t**>(rec + 0x18);
            for (int t = 0; t < kEcsTypeBits; ++t) {
                if (!(bits[t >> 6] >> (t & 63) & 1)) continue;
                float p[3];
                if (EcsPosition(rec, static_cast<uint16_t>(t), p) && PlausibleWorldPos(p)) ++counts[t];
            }
        });
        int best = 0;
        for (int t = 1; t < kEcsTypeBits; ++t) {
            if (counts[t] > counts[best]) best = t;
        }
        if (counts[best] < 100) return false;
        g_waterTypes.position = static_cast<uint16_t>(best);
    }
    if (!g_waterTypes.offer) {
        ForEachEcsEntity(reg, [&](uint8_t* rec) {
            if (g_waterTypes.offer) return;
            const auto* bits = *reinterpret_cast<const uint64_t**>(rec + 0x18);
            for (int t = 0; t < kEcsTypeBits; ++t) {
                if (!(bits[t >> 6] >> (t & 63) & 1)) continue;
                uint16_t size = 0;
                const uint8_t* c = EcsComponent(rec, static_cast<uint16_t>(t), &size);
                if (c && size == 0xc && *reinterpret_cast<const uint32_t*>(c) == kDrinkFromVerb) {
                    g_waterTypes.offer = static_cast<uint16_t>(t);
                    return;
                }
            }
        });
        if (!g_waterTypes.offer) return false; // nothing drinkable loaded yet: try again later
        if (g_log) g_log("Built water: registry %p, position type %u, interaction offer type %u", reg,
                         g_waterTypes.position, g_waterTypes.offer);
    }
    return true;
}

// Every drinkable entity's position (world water included). False if the registry is not readable yet.
bool ScanDrinkableWater(std::vector<BuiltWaterSpot>& out) {
    auto* reg = static_cast<uint8_t*>(g_waterRegistry.load(std::memory_order_relaxed));
    static void* loggedReg = nullptr;
    static int loggedState = -1;
    if (!reg) {
        // The first learned registry whose entities include something drinkable
        int seen = 0;
        for (auto& slot : g_waterRegistries) {
            auto* cand = static_cast<uint8_t*>(slot.load(std::memory_order_relaxed));
            if (!cand) continue;
            ++seen;
            bool ok = false;
            __try {
                ok = DetectWaterTypesInner(cand);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            if (ok) {
                reg = cand;
                g_waterRegistry.store(cand, std::memory_order_relaxed);
                break;
            }
        }
        if (!reg) {
            if (g_log && loggedState != seen) g_log("Built water: %d entity registries seen, none with drinkable water yet", seen);
            loggedState = seen;
            return false;
        }
    }
    __try {
        if (!DetectWaterTypesInner(reg)) {
            if (g_log && (loggedReg != reg || loggedState != 101)) {
                g_log("Built water: registry %p, types not found yet (position %u, offer %u)", reg, g_waterTypes.position,
                      g_waterTypes.offer);
            }
            loggedReg = reg;
            loggedState = 101;
            return false;
        }
        loggedState = 102;
        const uint16_t posType = g_waterTypes.position, offerType = g_waterTypes.offer;
        ForEachEcsEntity(reg, [&](uint8_t* rec) {
            uint16_t size = 0;
            const uint8_t* c = EcsComponent(rec, offerType, &size);
            if (!c || size != 0xc || *reinterpret_cast<const uint32_t*>(c) != kDrinkFromVerb) return;
            float p[3];
            if (EcsPosition(rec, posType, p) && PlausibleWorldPos(p) && out.size() < 512) out.push_back({p[0], p[1], p[2]});
        });
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// True near one of the world's wells or barrels (the Water Source row's resource rows).
bool IsWorldWater(float x, float z) {
    for (size_t d = 0; d < kOreDepositCount; ++d) {
        const OreDeposit& dep = kOreDeposits[d];
        if (dep.ore < kWaterFirst || dep.ore >= kWaterFirst + kWaterCount) continue;
        if (std::fabs(dep.x - x) < 2.5f && std::fabs(dep.z - z) < 2.5f) return true;
    }
    return false;
}

// Every few seconds in a world: adds the built water found near the player, forgets remembered water within 40 m of
// the player that is gone.
void UpdateBuiltWater(uint8_t* client) {
    static uint64_t lastTick = 0;
    const uint64_t now = GetTickCount64();
    if (g_lootSection.empty() || now - lastTick < 3000ULL) return;
    lastTick = now;
    std::vector<BuiltWaterSpot> found;
    if (!ScanDrinkableWater(found)) return;
    float px = 0.0f, pz = 0.0f;
    const bool havePlayer = GetHudPlayerWorldPos(client, px, pz);
    auto near2d = [](const BuiltWaterSpot& a, const BuiltWaterSpot& b) {
        return std::fabs(a.x - b.x) < 1.5f && std::fabs(a.z - b.z) < 1.5f && std::fabs(a.y - b.y) < 3.0f;
    };
    bool changed = false;
    for (const auto& f : found) {
        if (IsWorldWater(f.x, f.z)) continue;
        bool known = false;
        for (const auto& w : g_builtWater) known = known || near2d(w, f);
        if (!known) {
            g_builtWater.push_back(f);
            changed = true;
        }
    }
    if (havePlayer) {
        const size_t before = g_builtWater.size();
        g_builtWater.erase(std::remove_if(g_builtWater.begin(), g_builtWater.end(), [&](const BuiltWaterSpot& w) {
            if ((w.x - px) * (w.x - px) + (w.z - pz) * (w.z - pz) > 40.0f * 40.0f) return false;
            for (const auto& f : found) {
                if (near2d(w, f)) return false;
            }
            return true;
        }), g_builtWater.end());
        changed = changed || g_builtWater.size() != before;
    }
    if (changed) {
        g_builtWaterDirty = true;
        g_lootSaveDirty = true; // saved with the looted chests
        if (g_log) g_log("Built water: %zu in world %s", g_builtWater.size(), g_lootSection.c_str());
    }
}

void SaveBuiltWater(const std::string& path) {
    if (g_lootSection.empty()) return;
    std::string text;
    char buf[48];
    for (const auto& w : g_builtWater) {
        std::snprintf(buf, sizeof(buf), "%s%.0f,%.0f,%.0f", text.empty() ? "" : ";", w.x, w.y, w.z);
        text += buf;
    }
    WritePrivateProfileStringA(g_lootSection.c_str(), "BuiltWater", text.c_str(), path.c_str());
    g_builtWaterDirty = false;
}

void LoadBuiltWater(const char* section, const std::string& path) {
    g_builtWater.clear();
    std::vector<char> text(16 * 1024);
    GetPrivateProfileStringA(section, "BuiltWater", "", text.data(), static_cast<DWORD>(text.size()), path.c_str());
    const char* t = text.data();
    BuiltWaterSpot w{};
    int used = 0;
    while (*t && sscanf_s(t, "%f,%f,%f%n", &w.x, &w.y, &w.z, &used) == 3) {
        g_builtWater.push_back(w);
        t += used;
        if (*t == ';') ++t;
    }
}

// A world was left: its water is saved with its record; the next world's registry is learned again.
void ResetBuiltWater() {
    g_builtWater.clear();
    g_builtWaterDirty = false;
    g_waterRegistry.store(nullptr, std::memory_order_relaxed);
    for (auto& slot : g_waterRegistries) slot.store(nullptr, std::memory_order_relaxed);
    g_waterTypes = WaterEcsTypes{};
}
